import json
import base64
import os
from datetime import datetime, timezone

import boto3

bedrock = boto3.client("bedrock-runtime")
sns = boto3.client("sns")
s3 = boto3.client("s3")

SNS_TOPIC_ARN = os.environ["SNS_TOPIC_ARN"]
S3_BUCKET = os.environ["S3_BUCKET"]
BEDROCK_MODEL_ID = os.environ["BEDROCK_MODEL_ID"]


def lambda_handler(event, context):
    body = event.get("body", "")
    is_base64 = event.get("isBase64Encoded", False)

    if is_base64:
        image_bytes = base64.b64decode(body)
    else:
        image_bytes = body.encode("latin-1") if isinstance(body, str) else body

    # Save image to S3
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d/%H-%M-%S")
    s3_key = f"{timestamp}.jpg"
    s3.put_object(Bucket=S3_BUCKET, Key=s3_key, Body=image_bytes, ContentType="image/jpeg")

    image_b64 = base64.b64encode(image_bytes).decode("utf-8")

    response = bedrock.invoke_model(
        modelId=BEDROCK_MODEL_ID,
        contentType="application/json",
        accept="application/json",
        body=json.dumps(
            {
                "anthropic_version": "bedrock-2023-05-31",
                "max_tokens": 256,
                "messages": [
                    {
                        "role": "user",
                        "content": [
                            {
                                "type": "image",
                                "source": {
                                    "type": "base64",
                                    "media_type": "image/jpeg",
                                    "data": image_b64,
                                },
                            },
                            {
                                "type": "text",
                                "text": (
                                    "Look at this garage image. Is the garage door "
                                    "OPEN or CLOSED? Respond with only a JSON object: "
                                    '{"status": "open" or "closed", '
                                    '"confidence": "high" or "medium" or "low", '
                                    '"description": "brief description of what you see"}'
                                ),
                            },
                        ],
                    }
                ],
            }
        ),
    )

    result = json.loads(response["body"].read())
    answer_text = result["content"][0]["text"]

    try:
        start = answer_text.index("{")
        end = answer_text.rindex("}") + 1
        answer = json.loads(answer_text[start:end])
    except (ValueError, json.JSONDecodeError):
        answer = {
            "status": "unknown",
            "confidence": "low",
            "description": answer_text,
        }

    print(json.dumps({"timestamp": timestamp, "s3_key": s3_key, **answer}))

    if answer.get("status") == "open":
        sns.publish(
            TopicArn=SNS_TOPIC_ARN,
            Subject="Garage Door is OPEN",
            Message=(
                f"Your garage door appears to be OPEN.\n\n"
                f"Confidence: {answer.get('confidence', 'unknown')}\n"
                f"Description: {answer.get('description', 'N/A')}"
            ),
        )

    return {
        "statusCode": 200,
        "body": json.dumps(answer),
    }
